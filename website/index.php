<?php include("trick.php"); ?>
<!DOCTYPE HTML PUBLIC "-//W3C//DTD HTML 4.01 Transitional//EN" "http://www.w3.org/TR/html4/loose.dtd">
<html>
<head>
 <meta http-equiv="Content-Type" content="text/html; charset=iso-8859-1">
 <meta name="author" content="http://wiki.linux-ha.org">
 <title><?php echo "$pagetitle: $sitename"; ?></title>
 <link rel="stylesheet" href="/linuxha.css" type="text/css">
</head>
<body>
<?php browser_compatibility_messages(); ?>
<div id="i_site">
 <div id="i_header"><?php echo MoinMoin("TopLogo"); ?></div>
 <div id="i_menu"><?php echo MoinMoin("TopMenu"); ?></div>
 <div id="i_pagebody">
  <div id="i_sidebar">
   <div id="i_mainmenu"><?php echo MoinMoin("MainMenu"); ?></div>
   <div id="i_slashboxes"><?php echo MoinMoin("SlashBoxes"); ?></div>
   <div id="i_additional_actions">
     <?php echo '<a href="/print.php/$pagename">printer friendly view</a>'?>
     <?php echo '<a href="/print.php/$pagename">'?><IMG src="/img/moin-print.png" alt="printer"></a>
   </div>
  </div>
  <div id="i_content">
	<?php echo $content; ?>
  </div>
  <div id="i_footer"><?php echo MoinMoin("PageFooter"); ?></div>
 </div>
</div>
</body>
</html>
